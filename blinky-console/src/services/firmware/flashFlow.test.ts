import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { Transport, TransportEvent, TransportEventCallback } from '../transport';
import { PREFLIGHT_UPTIME_MS, STABLE_UPTIME_MS } from './deviceGates';
import { DeviceLink } from './deviceLink';
import { finishAfterFlash, preflight, type FlashUi } from './flashFlow';
import type { ReleaseEntry, ReleaseIndex, VerifiedRelease } from './releases';

/** A device on the console protocol whose uptime follows the (fake) clock. */
class FakeDevice implements Transport {
  bootedAt = Date.now();
  version = 'b214-aaaaaaaa';
  configured = true;
  sent: string[] = [];
  private listeners: TransportEventCallback[] = [];

  reboot() {
    this.bootedAt = Date.now();
  }
  isSupported() {
    return true;
  }
  isConnected() {
    return true;
  }
  async connect() {}
  async disconnect() {}
  addEventListener(cb: TransportEventCallback) {
    this.listeners.push(cb);
  }
  removeEventListener(cb: TransportEventCallback) {
    this.listeners = this.listeners.filter(l => l !== cb);
  }
  private reply(line: string) {
    queueMicrotask(() => this.listeners.forEach(l => l({ type: 'line', line } as TransportEvent)));
  }
  async send(cmd: string) {
    this.sent.push(cmd);
    if (cmd === 'json info') {
      this.reply('{"m":{"a":0}}'); // a stream line must not be taken for the reply
      this.reply(
        JSON.stringify({
          version: this.version,
          platform: 'nrf52840',
          sn: 'D63BD31605B36A15',
          ble: 'C2:11:22:33:44:55',
          millis: Date.now() - this.bootedAt,
          device: this.configured
            ? { id: 'hat_v1', name: 'Hat', configured: true }
            : { configured: false, safeMode: true },
          fps: 160,
          audioOverruns: 0,
          audioSamplesLost: 0,
        })
      );
    } else if (cmd === 'restore_runtime_settings' || cmd === 'save') {
      this.reply('OK');
    }
  }
}

function release(tag: string, settings = 70): ReleaseEntry {
  const file = { path: `${tag}/f`, sha256: '0'.repeat(64) };
  return {
    tag,
    build: Number(tag.slice(1)),
    version: `${tag}-aaaaaaaa`,
    git_sha: 'a'.repeat(40),
    settings_version: settings,
    device_version: 1,
    published_at: '2026-10-01T00:00:00Z',
    url: `https://github.com/x/y/releases/tag/${tag}`,
    files: { hex: file, uf2: file },
  };
}

const b214 = release('b214');
const b216 = release('b216');
const index: ReleaseIndex = { schema: 1, latest: 'b216', releases: [b216, b214] };
const verified = { entry: b216 } as VerifiedRelease;

function ui(confirm = true): FlashUi & { logs: string[] } {
  const logs: string[] = [];
  return {
    logs,
    log: m => logs.push(m),
    progress: () => {},
    ask: () => Promise.reject(new Error('unexpected ask')),
    confirm: vi.fn(async () => confirm),
    committed: () => {},
    signal: new AbortController().signal,
  };
}

beforeEach(() => vi.useFakeTimers());
afterEach(() => vi.useRealTimers());

describe('preflight', () => {
  it('waits out the reboot-counter window before allowing a reset', async () => {
    const dev = new FakeDevice();
    vi.advanceTimersByTime(60_000);
    const p = preflight(new DeviceLink(dev), verified, index, ui());
    await vi.advanceTimersByTimeAsync(PREFLIGHT_UPTIME_MS);
    const info = await p;
    expect(info.millis).toBeGreaterThanOrEqual(PREFLIGHT_UPTIME_MS);
  });

  it('refuses when the device restarts during the wait', async () => {
    const dev = new FakeDevice();
    const p = preflight(new DeviceLink(dev), verified, index, ui());
    const done = expect(p).rejects.toThrow(/restarted while waiting/);
    await vi.advanceTimersByTimeAsync(30_000);
    dev.reboot();
    await vi.advanceTimersByTimeAsync(20_000);
    await done;
  });

  it('refuses an unconfigured device without asking anything', async () => {
    const dev = new FakeDevice();
    dev.configured = false;
    const u = ui();
    await expect(preflight(new DeviceLink(dev), verified, index, u)).rejects.toThrow(
      /unconfigured/
    );
    expect(u.confirm).not.toHaveBeenCalled();
  });

  it('stops when the operator declines a confirmation', async () => {
    const dev = new FakeDevice();
    dev.version = 'b216-aaaaaaaa'; // reinstall needs confirming
    vi.advanceTimersByTime(PREFLIGHT_UPTIME_MS);
    await expect(preflight(new DeviceLink(dev), verified, index, ui(false))).rejects.toThrow(
      'Cancelled'
    );
  });
});

describe('finishAfterFlash', () => {
  it('verifies, restores settings and saves, then returns without holding for markStable', async () => {
    const dev = new FakeDevice();
    const before = { ...(await infoOf(dev)) };
    dev.reboot();
    dev.version = b216.version;
    const close = vi.fn(async () => {});
    const u = ui();
    const p = finishAfterFlash(new DeviceLink(dev), before, b216.version, u, close);
    await vi.advanceTimersByTimeAsync(100);
    await p;
    expect(dev.sent.filter(c => c !== 'json info')).toEqual(['restore_runtime_settings', 'save']);
    expect(Date.now() - dev.bootedAt).toBeLessThan(STABLE_UPTIME_MS);
    expect(u.logs.some(l => /Leave it powered for about \d+s/.test(l))).toBe(true);
    // The first minute is still watched in the background; the link closes after it.
    expect(close).not.toHaveBeenCalled();
    await vi.advanceTimersByTimeAsync(STABLE_UPTIME_MS + 10_000);
    expect(close).toHaveBeenCalledTimes(1);
    expect(u.logs).toContain('The new firmware passed its first minute');
  });

  it('fails on the wrong version, never saves, and closes the link', async () => {
    const dev = new FakeDevice();
    const before = await infoOf(dev);
    dev.reboot(); // came back on the old firmware
    const close = vi.fn(async () => {});
    const p = finishAfterFlash(new DeviceLink(dev), before, b216.version, ui(), close);
    const done = expect(p).rejects.toThrow(/expected b216-aaaaaaaa/);
    await vi.advanceTimersByTimeAsync(1000);
    await done;
    expect(dev.sent).not.toContain('save');
    expect(close).toHaveBeenCalledTimes(1);
  });

  it('reports a restart during the first minute after success', async () => {
    const dev = new FakeDevice();
    const before = await infoOf(dev);
    dev.reboot();
    dev.version = b216.version;
    const close = vi.fn(async () => {});
    const u = ui();
    const p = finishAfterFlash(new DeviceLink(dev), before, b216.version, u, close);
    await vi.advanceTimersByTimeAsync(100);
    await p;
    await vi.advanceTimersByTimeAsync(20_000);
    dev.reboot();
    await vi.advanceTimersByTimeAsync(10_000);
    expect(u.logs.some(l => /restarted .* during its first minute/.test(l))).toBe(true);
    expect(close).toHaveBeenCalledTimes(1);
  });
});

async function infoOf(dev: FakeDevice) {
  const p = new DeviceLink(dev).info();
  await vi.advanceTimersByTimeAsync(0);
  return p;
}
