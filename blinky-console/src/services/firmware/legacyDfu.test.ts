import { describe, expect, it } from 'vitest';
import { image, SimBootloader } from './__fixtures__/simBootloader';
import { DfuTransferError, PRN_INTERVAL, transferLegacyDfu } from './legacyDfu';

const FAST = { settleMs: 0, prnTimeoutMs: 50, receiveTimeoutMs: 50 };

describe('transferLegacyDfu', () => {
  it('sends the whole image and activates it', async () => {
    const sim = new SimBootloader();
    const img = image(1003); // not a multiple of 4: exercises tail padding
    await transferLegacyDfu(sim, img, undefined, FAST);
    expect(sim.received.slice(0, 1003)).toEqual([...img.firmware]);
    expect(sim.prn).toBe(PRN_INTERVAL);
    expect(sim.activated).toBe(true);
    expect(sim.resetCount).toBe(0);
    expect(sim.log[0]).toEqual([0x01, 0x04]); // START_DFU, application image
  });

  it('treats INVALID_STATE as a safe retry: nothing erased, bootloader reset', async () => {
    const sim = new SimBootloader({ startResult: 0x02 });
    const err = await transferLegacyDfu(sim, image(64), undefined, FAST).catch(e => e);
    expect(err).toBeInstanceOf(DfuTransferError);
    expect(err.invalidState).toBe(true);
    expect(err.eraseStarted).toBe(false);
    expect(sim.resetCount).toBe(1);
  });

  it('aborts on the first receipt when the link truncates packets', async () => {
    const sim = new SimBootloader({ deliverBytes: 18 });
    const err = await transferLegacyDfu(sim, image(4000), undefined, FAST).catch(e => e);
    expect(err).toBeInstanceOf(DfuTransferError);
    expect(err.message).toMatch(/received 144 bytes but 160 were sent/);
    expect(err.eraseStarted).toBe(true);
    expect(sim.packets).toBe(PRN_INTERVAL);
    expect(sim.resetCount).toBe(1);
    expect(sim.activated).toBe(false);
  });

  it('validates speculatively when the completion notification is lost', async () => {
    const sim = new SimBootloader({ loseReceiveResponse: true });
    await transferLegacyDfu(sim, image(500), undefined, FAST);
    expect(sim.activated).toBe(true);
  });

  it('honours cancel up to START_DFU: nothing erased, bootloader reset', async () => {
    const sim = new SimBootloader();
    const abort = new AbortController();
    abort.abort();
    const err = await transferLegacyDfu(sim, image(64), undefined, FAST, abort.signal).catch(
      e => e
    );
    expect(err.message).toBe('Cancelled');
    expect(err.eraseStarted).toBe(false);
    expect(sim.log).toEqual([[0x06]]); // only SYSTEM_RESET, never START_DFU
  });

  it('refuses to talk to the app instead of the bootloader, without resetting it', async () => {
    const sim = new SimBootloader({ revision: 1 });
    const err = await transferLegacyDfu(sim, image(64), undefined, FAST).catch(e => e);
    expect(err.message).toMatch(/not the bootloader/);
    expect(err.eraseStarted).toBe(false);
    expect(sim.log).toEqual([]);
    expect(sim.disconnected).toBe(true);
  });
});
