import { afterEach, describe, expect, it, vi } from 'vitest';
import type { FlashUi } from './flashFlow';
import type { VerifiedRelease } from './releases';
import { deliverUf2, parseInfoUf2, reconnectApp } from './usbFlash';

const INFO_OK =
  'UF2 Bootloader 0.8.0-10-g4c5faae lib/nrfx (v2.0.0)\r\nModel: Seeed XIAO nRF52840\r\n' +
  'Board-ID: nRF52840-SeeedXiaoSense-v1\r\nDate: May 20 2026\r\n';
const INFO_OLD_BL = INFO_OK.replace('0.8.0-10-g4c5faae', '0.6.1');

/** A UI that answers each prompt by running the option whose label matches. */
function ui(choose: (prompt: string, labels: string[]) => string) {
  const events: string[] = [];
  const u: FlashUi & { events: string[] } = {
    events,
    log: (m, level) => events.push(`log:${level ?? 'info'}:${m}`),
    progress: () => {},
    confirm: async () => true,
    committed: () => events.push('committed'),
    signal: new AbortController().signal,
    ask: async (prompt, options) => {
      const label = choose(
        prompt,
        options.map(o => o.label)
      );
      events.push(`ask:${label}`);
      const opt = options.find(o => o.label === label);
      if (!opt) throw new Error(`no option ${label}`);
      return opt.action();
    },
  };
  return u;
}

/** Make the next <input type=file>.click() "choose" a file with this content. */
function stubFilePick(name: string, text: string) {
  vi.spyOn(HTMLInputElement.prototype, 'click').mockImplementationOnce(function (
    this: HTMLInputElement
  ) {
    // jsdom's File has no .text(); browsers' does.
    const file = { name, text: async () => text };
    Object.defineProperty(this, 'files', { value: [file] });
    this.dispatchEvent(new Event('change'));
  });
}

const release = { entry: { tag: 'b216' }, uf2: new Uint8Array(512) } as unknown as VerifiedRelease;

afterEach(() => vi.restoreAllMocks());

describe('parseInfoUf2', () => {
  it('reads the fleet bootloader file', () => {
    expect(parseInfoUf2(INFO_OK)).toEqual({
      bootloader: '0.8.0-10-g4c5faae',
      boardId: 'nRF52840-SeeedXiaoSense-v1',
    });
  });
});

describe('deliverUf2, manual copy', () => {
  it('checks INFO_UF2.TXT and commits before handing out the file', async () => {
    // jsdom has no object URLs.
    URL.createObjectURL = vi.fn(() => 'blob:x');
    URL.revokeObjectURL = vi.fn();
    stubFilePick('INFO_UF2.TXT', INFO_OK);
    const u = ui((_p, labels) =>
      labels.includes('Copy the file myself') ? 'Copy the file myself' : labels[0]
    );
    await deliverUf2(release, u);
    const i = u.events.indexOf('committed');
    expect(i).toBeGreaterThan(-1);
    expect(i).toBeLessThan(u.events.indexOf('ask:Download blinky-things-b216.uf2'));
  });

  it('refuses an unsupported bootloader before offering the file', async () => {
    stubFilePick('INFO_UF2.TXT', INFO_OLD_BL);
    let round = 0;
    const u = ui(() => {
      if (++round > 1) throw new Error('stop'); // the page asks again after the refusal
      return 'Copy the file myself';
    });
    await expect(deliverUf2(release, u)).rejects.toThrow('stop');
    expect(u.events.some(e => e.includes('only updates devices on 0.8.0-10'))).toBe(true);
    expect(u.events).not.toContain('committed');
    expect(u.events.some(e => e.startsWith('ask:Download'))).toBe(false);
  });
});

describe('reconnectApp', () => {
  it('without an expected serial (recovery) never auto-picks a permitted port', async () => {
    const getPorts = vi.fn(async () => []);
    vi.stubGlobal('navigator', { ...navigator, serial: { getPorts, requestPort: vi.fn() } });
    const u = ui(() => {
      throw new Error('asked');
    });
    await expect(reconnectApp(null, u)).rejects.toThrow('asked');
    expect(getPorts).not.toHaveBeenCalled();
    vi.unstubAllGlobals();
  });
});

describe('deliverUf2, drive write', () => {
  function driveWith(write: () => Promise<void>, close: () => Promise<void> = async () => {}) {
    const info = { getFile: async () => ({ text: async () => INFO_OK }) };
    const uf2 = { createWritable: async () => ({ write, close }) };
    return {
      name: 'XIAO-SENSE',
      getFileHandle: async (name: string) => (name === 'INFO_UF2.TXT' ? info : uf2),
    } as unknown as FileSystemDirectoryHandle;
  }

  it('treats a write torn by the bootloader reset as "check the device", not a failure', async () => {
    window.showDirectoryPicker = async () =>
      driveWith(async () => {
        throw new DOMException('device gone', 'NotFoundError');
      });
    const u = ui(() => 'Choose the XIAO-SENSE drive');
    await expect(deliverUf2(release, u)).resolves.toBeUndefined();
    expect(u.events).toContain('committed');
    expect(u.events.some(e => e.startsWith('log:warn:The drive went away'))).toBe(true);
    delete window.showDirectoryPicker;
  });

  it('fails at once when the browser refuses the write', async () => {
    window.showDirectoryPicker = async () =>
      driveWith(async () => {
        throw new DOMException('no', 'NotAllowedError');
      });
    let round = 0;
    const u = ui(() => {
      if (++round > 1) throw new Error('stop');
      return 'Choose the XIAO-SENSE drive';
    });
    await expect(deliverUf2(release, u)).rejects.toThrow('stop');
    expect(u.events.some(e => e.includes('refused to write'))).toBe(true);
    delete window.showDirectoryPicker;
  });
});
