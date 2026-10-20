import { describe, expect, it } from 'vitest';
import golden from './__fixtures__/golden.json';
import { FirmwareError } from './errors';
import { APP_END, APP_START, UF2_FAMILY_NRF52840_APP } from './flashLayout';
import { parseIntelHex } from './intelHex';
import { assertUf2MatchesHex, buildLegacyDfuImage, crc16 } from './legacyDfuPackage';
import { validateUf2 } from './uf2';

// Golden values come from the Python implementations these modules port
// (__fixtures__/make_fixtures.py: compile.crc16, generate_dfu_package's init
// packet layout, and uf2conv.py as release packaging runs it).
const hexText: string = golden.appHex;
const uf2Bytes = Uint8Array.from(atob(golden.appUf2Base64), c => c.charCodeAt(0));

const fromHex = (s: string) => Uint8Array.from(s.match(/../g) ?? [], b => parseInt(b, 16));
const toHex = (b: Uint8Array) => Array.from(b, x => x.toString(16).padStart(2, '0')).join('');

function rec(addr: number, type: number, data: number[]): string {
  const b = [data.length, (addr >> 8) & 0xff, addr & 0xff, type, ...data];
  const sum = (0x100 - (b.reduce((a, x) => a + x, 0) & 0xff)) & 0xff;
  return (
    ':' +
    [...b, sum]
      .map(x => x.toString(16).padStart(2, '0'))
      .join('')
      .toUpperCase()
  );
}
const ELA_0002 = rec(0, 0x04, [0x00, 0x02]);
const EOF = rec(0, 0x01, []);

describe('crc16', () => {
  it.each(Object.entries(golden.crcVectors as Record<string, { hex: string; crc: number }>))(
    'matches compile.crc16 for %s',
    (_name, v) => {
      expect(crc16(fromHex(v.hex))).toBe(v.crc);
    }
  );
});

describe('parseIntelHex', () => {
  it('produces the objcopy binary (gaps zero-filled)', () => {
    const img = parseIntelHex(hexText);
    expect(img.start).toBe(golden.start);
    expect(img.end).toBe(golden.end);
    expect(img.bin.length).toBe(golden.binLength);
    expect(img.bin.subarray(0x800, 0x840).every(b => b === 0)).toBe(true);
  });

  it('rejects a bad checksum', () => {
    const bad = [ELA_0002, rec(0x7000, 0, [1, 2, 3]).slice(0, -2) + '00', EOF].join('\n');
    expect(() => parseIntelHex(bad)).toThrow(/checksum/);
  });

  it('rejects a file without EOF (truncated download)', () => {
    expect(() => parseIntelHex([ELA_0002, rec(0x7000, 0, [1])].join('\n'))).toThrow(/EOF/);
  });

  it('rejects data after EOF', () => {
    expect(() =>
      parseIntelHex([ELA_0002, rec(0x7000, 0, [1]), EOF, rec(0x7001, 0, [2])].join('\n'))
    ).toThrow(/after the EOF/);
  });

  it('rejects overlapping records', () => {
    expect(() =>
      parseIntelHex([ELA_0002, rec(0x7000, 0, [1, 2]), rec(0x7001, 0, [3]), EOF].join('\n'))
    ).toThrow(/overlaps/);
  });

  it('rejects unknown record types', () => {
    expect(() => parseIntelHex([rec(0, 0x07, [1]), EOF].join('\n'))).toThrow(/record type/);
  });
});

describe('buildLegacyDfuImage', () => {
  it('matches generate_dfu_package byte for byte', () => {
    const dfu = buildLegacyDfuImage(parseIntelHex(hexText));
    expect(toHex(dfu.initPacket)).toBe(golden.initPacket);
    expect(crc16(dfu.firmware)).toBe(golden.crc16);
  });

  it('refuses an image that does not start at the application start', () => {
    const hex = [ELA_0002, rec(0x6000, 0, [1, 2, 3, 4]), EOF].join('\n');
    expect(() => buildLegacyDfuImage(parseIntelHex(hex))).toThrow(/must start at 0x00027000/);
  });

  it('refuses an image that runs past the application region', () => {
    const img = {
      start: APP_START,
      end: APP_END + 4,
      bin: new Uint8Array(APP_END + 4 - APP_START),
    };
    expect(() => buildLegacyDfuImage(img)).toThrow(/past the application region/);
  });
});

describe('validateUf2', () => {
  it('accepts the uf2conv.py release image', () => {
    const blocks = validateUf2(uf2Bytes);
    expect(blocks.length).toBe(uf2Bytes.length / 512);
    expect(blocks[0].addr).toBe(APP_START);
  });

  const mutate = (fn: (v: DataView) => void) => {
    const copy = uf2Bytes.slice();
    fn(new DataView(copy.buffer));
    return copy;
  };

  it('refuses a block aimed at the SoftDevice', () => {
    expect(() => validateUf2(mutate(v => v.setUint32(12, 0x26000, true)))).toThrow(
      /outside the application region/
    );
  });

  it('refuses a block aimed at config / bootloader flash', () => {
    expect(() => validateUf2(mutate(v => v.setUint32(12, 0xf4000, true)))).toThrow(
      /outside the application region/
    );
    expect(() => validateUf2(mutate(v => v.setUint32(12, 0xed000, true)))).toThrow(
      /outside the application region/
    );
  });

  it('refuses a bootloader-family image', () => {
    expect(() => validateUf2(mutate(v => v.setUint32(28, 0xd663823c, true)))).toThrow(/family/);
  });

  it('refuses inconsistent block numbering', () => {
    expect(() => validateUf2(mutate(v => v.setUint32(20, 1, true)))).toThrow(/numbered/);
  });

  it('refuses a truncated file and bad magic', () => {
    expect(() => validateUf2(uf2Bytes.subarray(0, 700))).toThrow(/multiple of 512/);
    expect(() => validateUf2(mutate(v => v.setUint32(0, 0, true)))).toThrow(/magic/);
  });

  it('uses the nRF52840 app family', () => {
    expect(new DataView(uf2Bytes.buffer, uf2Bytes.byteOffset).getUint32(28, true)).toBe(
      UF2_FAMILY_NRF52840_APP
    );
  });
});

describe('assertUf2MatchesHex', () => {
  it('accepts the uf2 built from the same hex', () => {
    expect(() => assertUf2MatchesHex(validateUf2(uf2Bytes), parseIntelHex(hexText))).not.toThrow();
  });

  it('refuses a one-byte difference', () => {
    const copy = uf2Bytes.slice();
    copy[32 + 100] ^= 0xff;
    expect(() => assertUf2MatchesHex(validateUf2(copy), parseIntelHex(hexText))).toThrow(
      FirmwareError
    );
  });

  it('refuses a uf2 missing part of the hex', () => {
    const blocks = validateUf2(uf2Bytes).slice(0, -1);
    expect(() => assertUf2MatchesHex(blocks, parseIntelHex(hexText))).toThrow(
      /missing from the UF2/
    );
  });
});
