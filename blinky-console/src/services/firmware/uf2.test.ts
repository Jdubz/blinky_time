import { describe, expect, it } from 'vitest';
import golden from './__fixtures__/golden.json';
import { APP_START, UF2_FAMILY_NRF52840_APP } from './flashLayout';
import { validateUf2 } from './uf2';

// A real uf2conv.py image that tools/uf2_upload.py validate_uf2 accepts
// (__fixtures__/make_fixtures.py).
const uf2Bytes = Uint8Array.from(atob(golden.appUf2Base64), c => c.charCodeAt(0));

describe('validateUf2', () => {
  it('accepts the uf2conv.py release image', () => {
    const blocks = validateUf2(uf2Bytes);
    expect(blocks.length).toBe(uf2Bytes.length / 512);
    expect(blocks[0].addr).toBe(APP_START);
    expect(blocks[0].addr).toBe(golden.appStart);
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
