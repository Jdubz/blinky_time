import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { image, SimBootloader } from './__fixtures__/simBootloader';
import { dfuWithRetries } from './bleFlash';
import type { FlashUi } from './flashFlow';
import type { VerifiedRelease } from './releases';

function ui(): FlashUi & { logs: string[]; commits: number; abort: AbortController } {
  const abort = new AbortController();
  const logs: string[] = [];
  const u = {
    abort,
    logs,
    commits: 0,
    log: (m: string) => logs.push(m),
    progress: () => {},
    ask: () => Promise.reject(new Error('unexpected ask')),
    confirm: async () => true,
    committed: () => {
      u.commits++;
    },
    signal: abort.signal,
  };
  return u;
}

const release = { dfu: image(4000) } as unknown as VerifiedRelease;
const device = {} as BluetoothDevice;

beforeEach(() => vi.useFakeTimers());
afterEach(() => vi.useRealTimers());

/** Run with fake time flowing until the promise settles. */
async function settle<T>(p: Promise<T>): Promise<T | Error> {
  const out = p.then(
    v => v,
    e => e as Error
  );
  for (let i = 0; i < 200; i++) await vi.advanceTimersByTimeAsync(1000);
  return out;
}

describe('dfuWithRetries', () => {
  it('remembers an erase from an earlier attempt when later attempts fail to connect', async () => {
    const u = ui();
    let calls = 0;
    const connect = async () => {
      calls++;
      if (calls === 1) return new SimBootloader({ deliverBytes: 18 }); // erases, then fails
      throw new Error('GATT connect failed');
    };
    const err = await settle(dfuWithRetries(device, release, u, connect));
    expect(calls).toBe(3);
    expect((err as Error).message).toMatch(/waiting in its bootloader/);
    expect((err as Error).message).not.toMatch(/not changed|Nothing was erased/);
  });

  it('stops at once on INVALID_STATE: nothing erased, no retry against a vanished bootloader', async () => {
    const u = ui();
    const connect = vi.fn(async () => new SimBootloader({ startResult: 0x02 }));
    const err = await settle(dfuWithRetries(device, release, u, connect));
    expect(connect).toHaveBeenCalledTimes(1);
    expect((err as Error).message).toMatch(/returned to its current firmware; nothing was changed/);
  });

  it('cancel before START_DFU writes nothing and does not retry', async () => {
    const u = ui();
    const sim = new SimBootloader();
    const connect = vi.fn(async () => sim);
    u.abort.abort();
    const err = await settle(dfuWithRetries(device, release, u, connect));
    expect((err as Error).message).toMatch(/Cancelled/);
    expect(sim.log.some(c => c[0] === 0x01)).toBe(false);
    expect(u.commits).toBe(0);
  });

  it('succeeds on a later attempt after a pre-erase connect failure', async () => {
    const u = ui();
    let calls = 0;
    const sim = new SimBootloader();
    const connect = async () => {
      if (++calls === 1) throw new Error('GATT connect failed');
      return sim;
    };
    const res = await settle(dfuWithRetries(device, release, u, connect));
    expect(res).toBeUndefined();
    expect(sim.activated).toBe(true);
    expect(u.commits).toBe(1);
  });
});
