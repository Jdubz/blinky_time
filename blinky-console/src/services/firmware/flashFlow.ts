import {
  postFlashCheck,
  preflightBlockers,
  requiredConfirmations,
  STABLE_UPTIME_MS,
  uptimeWaitMs,
  type Confirmation,
  type FlashInfo,
} from './deviceGates';
import type { DeviceLink } from './deviceLink';
import { FirmwareError } from './errors';
import type { ReleaseIndex, VerifiedRelease } from './releases';

/**
 * The page-facing side of a flash. Browsers only open device pickers (USB
 * port, bootloader drive, INFO_UF2.TXT) from a click, so every picker is
 * behind `ask`: the UI shows the prompt and runs the chosen option's action
 * inside the click handler.
 */
export interface FlashUi {
  log(message: string, level?: 'info' | 'warn' | 'error'): void;
  /** fraction in [0, 1], or null for "working, no estimate". */
  progress(label: string, fraction: number | null): void;
  ask<T>(prompt: string, options: { label: string; action: () => Promise<T> }[]): Promise<T>;
  /** Show these confirmations; resolve true only if the operator accepted all of them. */
  confirm(items: Confirmation[]): Promise<boolean>;
  /**
   * The point of no return is passed: from here the page must stay open
   * (wake lock, unload warning) and cancel is no longer offered.
   */
  committed(): void;
  readonly signal: AbortSignal;
}

export const sleep = (ms: number, signal?: AbortSignal) =>
  new Promise<void>((resolve, reject) => {
    const t = setTimeout(resolve, ms);
    signal?.addEventListener('abort', () => {
      clearTimeout(t);
      reject(new FirmwareError('Cancelled'));
    });
  });

export function checkAborted(ui: FlashUi): void {
  if (ui.signal.aborted) throw new FirmwareError('Cancelled');
}

/**
 * Everything before the device is touched: identity and config gates,
 * operator confirmations, and the reboot-counter uptime window. Returns the
 * `json info` snapshot the post-flash checks compare against.
 */
export async function preflight(
  link: DeviceLink,
  release: VerifiedRelease,
  index: ReleaseIndex,
  ui: FlashUi
): Promise<FlashInfo> {
  let info = await link.info();
  ui.log(
    `Device: ${info.device.name ?? info.device.id ?? '?'} (${info.device.id ?? 'no id'}), ` +
      `sn ${info.sn}, firmware ${info.version}, up ${Math.round(info.millis / 1000)}s`
  );
  const blockers = preflightBlockers(info);
  if (blockers.length)
    throw new FirmwareError(`This device can't be updated here: ${blockers.join('; ')}`);

  const confirmations = requiredConfirmations(info, release.entry, index);
  if (confirmations.length && !(await ui.confirm(confirmations))) {
    throw new FirmwareError('Cancelled');
  }

  // Entering the bootloader is a reset. Firmware up to b190 counts every boot
  // toward a 5-boot config quarantine until 5 min of uptime clears it.
  for (let wait = uptimeWaitMs(info); wait > 0; wait = uptimeWaitMs(info)) {
    ui.progress(
      `Waiting for the device to settle (${Math.ceil(wait / 1000)}s left — don't unplug or reset it)`,
      null
    );
    await sleep(Math.min(wait + 500, 10_000), ui.signal);
    const next = await link.info();
    if (next.millis < info.millis) {
      throw new FirmwareError(
        'The device restarted while waiting. Leave it running for 5 minutes, then try again.'
      );
    }
    info = next;
  }
  checkAborted(ui);
  return info;
}

/**
 * deploy_local.py steps 7–9 after the device is back on its app: verify it
 * is the same configured device on exactly the expected version with BLE up
 * (BEFORE `save`, which would hang on a half-up SoftDevice), restore runtime
 * settings + save as every fleet deploy does, then hold until markStable().
 */
export async function finishAfterFlash(
  link: DeviceLink,
  before: FlashInfo | null,
  expectedVersion: string,
  ui: FlashUi
): Promise<FlashInfo> {
  ui.progress('Checking the updated device', null);
  // deploy.sh: let the 5 s loop-metrics window close so fps is real.
  await sleep(6000);
  const after = await link.info();
  // A recovery (device found already in its bootloader) has no "before":
  // identity can't be compared, everything else still is.
  const { failures, warnings } = postFlashCheck(before ?? after, after, expectedVersion);
  warnings.forEach(w => ui.log(w, 'warn'));
  if (failures.length) throw new FirmwareError(`The update did not verify: ${failures.join('; ')}`);
  ui.log(`Device reports ${after.version}`);

  await link.command('restore_runtime_settings');
  await link.command('save');
  ui.log('Runtime settings restored to the release defaults and saved');

  let latest = after;
  for (let up = after.millis; up < STABLE_UPTIME_MS; ) {
    ui.progress(
      `Letting the new firmware settle (${Math.ceil((STABLE_UPTIME_MS - up) / 1000)}s)`,
      up / STABLE_UPTIME_MS
    );
    await sleep(Math.min(STABLE_UPTIME_MS - up + 1000, 5000));
    const now = await link.info();
    if (now.millis < up) {
      throw new FirmwareError(
        `The device restarted ${Math.round(now.millis / 1000)}s ago, during its first minute on the new ` +
          'firmware. Leave it powered and contact support before trying again.'
      );
    }
    up = now.millis;
    latest = now;
  }
  return latest;
}
