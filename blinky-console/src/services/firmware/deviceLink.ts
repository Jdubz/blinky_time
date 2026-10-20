import type { Transport, TransportEvent } from '../transport';
import { parseFlashInfo, type FlashInfo } from './deviceGates';
import { FirmwareError } from './errors';

/**
 * Strict request/response over a connected transport (USB serial)
 * for the flasher. Unlike DeviceProtocol.sendAndReceiveJson it matches the
 * reply by shape, so a stray stream line can't be mistaken for it, and every
 * failure throws.
 */
export class DeviceLink {
  constructor(private readonly transport: Transport) {}

  private waitFor<T>(
    cmd: string,
    timeoutMs: number,
    match: (line: string) => T | undefined
  ): Promise<T> {
    return new Promise((resolve, reject) => {
      const done = (fn: () => void) => {
        clearTimeout(timer);
        this.transport.removeEventListener(onEvent);
        fn();
      };
      const onEvent = (e: TransportEvent) => {
        if (e.type === 'line' && e.line !== undefined) {
          const got = match(e.line);
          if (got !== undefined) done(() => resolve(got));
        } else if (e.type === 'disconnected') {
          done(() =>
            reject(new FirmwareError(`The device disconnected while waiting for \`${cmd}\``))
          );
        }
      };
      const timer = setTimeout(
        () =>
          done(() =>
            reject(new FirmwareError(`No reply to \`${cmd}\` within ${timeoutMs / 1000}s`))
          ),
        timeoutMs
      );
      this.transport.addEventListener(onEvent);
      this.transport
        .send(cmd)
        .catch(err =>
          done(() => reject(new FirmwareError(`Could not send \`${cmd}\`: ${String(err)}`)))
        );
    });
  }

  /** `json info`, validated. */
  async info(timeoutMs = 5000): Promise<FlashInfo> {
    const raw = await this.waitFor('json info', timeoutMs, line => {
      if (!line.startsWith('{') || !line.includes('"millis"')) return undefined;
      try {
        const obj = JSON.parse(line) as Record<string, unknown>;
        return 'version' in obj ? obj : undefined;
      } catch {
        return undefined;
      }
    });
    return parseFlashInfo(raw);
  }

  /** A console command acknowledged by an `OK` line; an `ERROR` line fails it. */
  async command(cmd: string, timeoutMs = 5000): Promise<void> {
    const reply = await this.waitFor(cmd, timeoutMs, line =>
      line === 'OK' || line.startsWith('ERROR') ? line : undefined
    );
    if (reply !== 'OK') throw new FirmwareError(`\`${cmd}\` failed: ${reply}`);
  }

  /** Send without waiting for a reply (the device resets on `bootloader`). */
  async fire(cmd: string): Promise<void> {
    try {
      await this.transport.send(cmd);
    } catch (err) {
      throw new FirmwareError(`Could not send \`${cmd}\`: ${String(err)}`);
    }
  }
}
