import { describe, expect, it } from 'vitest';
import {
  parseFlashInfo,
  postFlashCheck,
  preflightBlockers,
  PREFLIGHT_UPTIME_MS,
  requiredConfirmations,
  uptimeWaitMs,
  type FlashInfo,
} from './deviceGates';
import type { ReleaseEntry, ReleaseIndex } from './releases';

function info(overrides: Partial<FlashInfo> = {}): FlashInfo {
  return parseFlashInfo({
    version: 'b214-aaaaaaaa',
    platform: 'nrf52840',
    sn: 'D63BD31605B36A15',
    ble: 'C2:11:22:33:44:55',
    millis: PREFLIGHT_UPTIME_MS + 1,
    device: { id: 'hat_v1', name: 'Hat', configured: true },
    fps: 160,
    audioOverruns: 0,
    audioSamplesLost: 0,
    ...overrides,
  });
}

function release(tag: string, settings: number): ReleaseEntry {
  const n = Number(tag.slice(1));
  const file = { path: `${tag}/f`, sha256: '0'.repeat(64) };
  return {
    tag,
    build: n,
    version: `${tag}-${'a'.repeat(8)}`,
    git_sha: 'a'.repeat(40),
    settings_version: settings,
    device_version: 1,
    published_at: '2026-10-01T00:00:00Z',
    url: `https://github.com/x/y/releases/tag/${tag}`,
    files: { hex: file, uf2: file },
  };
}

const b214 = release('b214', 70);
const b216 = release('b216', 70);
const b218 = release('b218', 71);
const index: ReleaseIndex = { schema: 1, latest: 'b218', releases: [b218, b216, b214] };

describe('parseFlashInfo', () => {
  it('rejects a reply that is not blinky firmware', () => {
    expect(() => parseFlashInfo({ version: 'x' })).toThrow(/missing/);
  });
});

describe('preflightBlockers', () => {
  it('passes a configured nRF52840', () => {
    expect(preflightBlockers(info())).toEqual([]);
  });

  it('refuses an unconfigured device, other platforms and a missing serial', () => {
    expect(preflightBlockers(info({ device: { configured: false } }))[0]).toMatch(/unconfigured/);
    expect(preflightBlockers(info({ platform: 'esp32s3' }))[0]).toMatch(/not nrf52840/);
    expect(preflightBlockers(info({ sn: '' }))[0]).toMatch(/serial/);
  });
});

describe('uptimeWaitMs', () => {
  it('waits out the reboot-counter window', () => {
    expect(uptimeWaitMs(info({ millis: 60_000 }))).toBe(PREFLIGHT_UPTIME_MS - 60_000);
    expect(uptimeWaitMs(info())).toBe(0);
  });
});

describe('requiredConfirmations', () => {
  const v = (r: ReleaseEntry) => info({ version: r.version });

  it('needs nothing for an upgrade within one settings version', () => {
    expect(requiredConfirmations(v(b214), b216, index)).toEqual([]);
  });

  it('flags a settings-format change', () => {
    expect(requiredConfirmations(v(b216), b218, index).map(c => c.kind)).toEqual([
      'settings-reset',
    ]);
  });

  it('flags downgrades and reinstalls', () => {
    expect(requiredConfirmations(v(b216), b214, index).map(c => c.kind)).toEqual(['downgrade']);
    expect(requiredConfirmations(v(b216), b216, index).map(c => c.kind)).toEqual(['reinstall']);
  });

  it('assumes a settings reset when the running version is not a published release', () => {
    const c = requiredConfirmations(info({ version: 'b217-dev-abcdef12' }), b216, index);
    expect(c.map(x => x.kind)).toEqual(['downgrade', 'settings-reset']);
  });
});

describe('postFlashCheck', () => {
  const before = info();
  const expected = 'b216-aaaaaaaa';

  it('passes the same device on the expected version', () => {
    expect(postFlashCheck(before, info({ version: expected }), expected)).toEqual({
      failures: [],
      warnings: [],
    });
  });

  it('fails on a version mismatch, another chip, lost config, or BLE down', () => {
    const r = postFlashCheck(
      before,
      info({ version: 'b214-aaaaaaaa', sn: 'FFFF', device: { configured: false }, ble: '' }),
      expected
    );
    expect(r.failures.join('\n')).toMatch(/expected b216/);
    expect(r.failures.join('\n')).toMatch(/different chip/);
    expect(r.failures.join('\n')).toMatch(/unconfigured/);
    expect(r.failures.join('\n')).toMatch(/Bluetooth did not start/);
  });

  it('warns on low fps and overruns', () => {
    const r = postFlashCheck(
      before,
      info({ version: expected, fps: 12, audioOverruns: 9 }),
      expected
    );
    expect(r.failures).toEqual([]);
    expect(r.warnings).toHaveLength(2);
  });
});
