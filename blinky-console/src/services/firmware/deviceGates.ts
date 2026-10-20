import { z } from 'zod';
import { FirmwareError } from './errors';
import { buildOf, type ReleaseEntry, type ReleaseIndex } from './releases';

/**
 * Pre-flight and post-flash gates, ported from tools/deploy_local.py so a
 * browser flash is held to the same bar as a workstation flash.
 */

/** b190-era firmware counts every boot toward the 5-boot config quarantine until 5 min of uptime. */
export const PREFLIGHT_UPTIME_MS = 5 * 60_000 + 15_000;
/** CLAUDE.md 60-second rule: markStable() fires at 60 s; wait 75 s before any further reset. */
export const STABLE_UPTIME_MS = 75_000;

/** `json info`, keeping the fields the gates read (DeviceInfoSchema strips them). */
export const FlashInfoSchema = z
  .object({
    version: z.string().min(1),
    platform: z.string(),
    sn: z.string(),
    ble: z.string(),
    millis: z.number().nonnegative(),
    device: z
      .object({
        configured: z.boolean(),
        id: z.string().optional(),
        name: z.string().optional(),
      })
      .passthrough(),
    fps: z.number().optional(),
    audioOverruns: z.number().optional(),
    audioSamplesLost: z.number().optional(),
  })
  .passthrough();
export type FlashInfo = z.infer<typeof FlashInfoSchema>;

export function parseFlashInfo(raw: unknown): FlashInfo {
  const parsed = FlashInfoSchema.safeParse(raw);
  if (!parsed.success) {
    const issue = parsed.error.issues[0];
    throw new FirmwareError(
      `The device's \`json info\` reply is missing ${issue?.path.join('.') || 'fields'} — ` +
        'is this blinky firmware?'
    );
  }
  return parsed.data;
}

/** Hard refusals: the device must not be flashed from here. */
export function preflightBlockers(info: FlashInfo): string[] {
  const out: string[] = [];
  if (info.platform !== 'nrf52840') out.push(`platform is "${info.platform}", not nrf52840`);
  if (!info.sn) out.push('the device reports no hardware serial number');
  if (!info.device.configured) {
    out.push(
      'the device is unconfigured (safe mode). A firmware update does not fix that, and its ' +
        'post-flash checks need a configured device. Get its configuration restored first.'
    );
  }
  return out;
}

/** Milliseconds to wait before a reset is safe (0 = now). */
export function uptimeWaitMs(info: FlashInfo): number {
  return Math.max(0, PREFLIGHT_UPTIME_MS - info.millis);
}

export type Confirmation =
  | { kind: 'reinstall'; message: string }
  | { kind: 'downgrade'; message: string }
  | { kind: 'settings-reset'; message: string };

/**
 * What the operator must explicitly accept before flashing `target` over the
 * device's current firmware. Empty means a plain upgrade within the same
 * SETTINGS_VERSION.
 */
export function requiredConfirmations(
  info: FlashInfo,
  target: ReleaseEntry,
  index: ReleaseIndex
): Confirmation[] {
  const out: Confirmation[] = [];
  const current = buildOf(info.version);
  if (info.version === target.version) {
    out.push({ kind: 'reinstall', message: `The device already runs ${target.version}.` });
  } else if (current !== null && current > target.build) {
    out.push({
      kind: 'downgrade',
      message: `This installs an older release (${target.tag}) over ${info.version}.`,
    });
  }
  const currentEntry = index.releases.find(r => r.version === info.version);
  if (!currentEntry) {
    out.push({
      kind: 'settings-reset',
      message:
        `${info.version} is not one of the published stable releases, so this page can't tell ` +
        'whether its saved tuning survives. Assume runtime settings reset to defaults.',
    });
  } else if (currentEntry.settings_version !== target.settings_version) {
    out.push({
      kind: 'settings-reset',
      message:
        `Settings format changes (${currentEntry.settings_version} → ${target.settings_version}): ` +
        'saved runtime tuning resets to defaults. The device configuration (LED layout) is kept.',
    });
  }
  return out;
}

export interface PostFlashResult {
  failures: string[];
  warnings: string[];
}

/**
 * deploy_local.py verify(): the device must come back on exactly the expected
 * version, as the same chip and device, configured, with BLE up (an empty
 * `ble` means the SoftDevice is half-up and the next flash write would hang,
 * so this is checked before `save`).
 */
export function postFlashCheck(
  before: FlashInfo,
  after: FlashInfo,
  expectedVersion: string
): PostFlashResult {
  const failures: string[] = [];
  const warnings: string[] = [];
  if (after.version !== expectedVersion) {
    failures.push(`device reports ${after.version}, expected ${expectedVersion}`);
  }
  if (after.sn.toUpperCase() !== before.sn.toUpperCase()) {
    failures.push(`a different chip answered (sn ${after.sn}, expected ${before.sn})`);
  }
  if (after.device.id !== before.device.id) {
    failures.push(
      `device id is ${after.device.id ?? 'missing'}, was ${before.device.id} (identity lost?)`
    );
  }
  if (!after.device.configured)
    failures.push('device came up unconfigured (config lost or quarantined)');
  if (after.audioOverruns === undefined || after.audioSamplesLost === undefined) {
    failures.push('missing audio overrun counters');
  }
  if (!after.ble) {
    failures.push(
      'Bluetooth did not start (empty `ble`). Do not change settings; reconnect over USB and contact support.'
    );
  }
  if (after.fps === undefined || after.fps === 0) warnings.push('fps not measured yet');
  else if (after.fps < 30) warnings.push(`low frame rate (${after.fps.toFixed(1)} fps)`);
  if ((after.audioOverruns ?? 0) > 5) warnings.push(`${after.audioOverruns} audio overruns`);
  return { failures, warnings };
}
