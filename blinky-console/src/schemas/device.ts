/**
 * Device information Zod schemas
 *
 * Validates responses from the `json info` command.
 * Updated for v28+ firmware with runtime device configuration support.
 */

import { z } from 'zod';

/**
 * Device configuration when properly configured (v28+)
 */
const ConfiguredDeviceSchema = z.object({
  id: z.string().min(1),
  name: z.string().min(1),
  width: z.number().int().positive(),
  height: z.number().int().positive(),
  leds: z.number().int().positive(),
  configured: z.literal(true),
});

/**
 * Device configuration in safe mode (not configured)
 */
const UnconfiguredDeviceSchema = z.object({
  configured: z.literal(false),
  safeMode: z.literal(true),
});

/**
 * Device configuration - either configured or in safe mode
 */
const DeviceConfigSchema = z.union([ConfiguredDeviceSchema, UnconfiguredDeviceSchema]);

/**
 * Device information from `json info` command (v28+ format).
 *
 * `sn` is the firmware-reported hardware serial number (FICR DEVICEID on
 * nRF52840). It matches the `hardware_sn` field on the server side and is
 * the canonical cross-transport identity — use it to deduplicate the same
 * physical device reached via WebSerial, Web Bluetooth, and/or a
 * blinky-server WebSocket proxy. Note: `device.id` is the *device-type*
 * (e.g. `bucket_v1`), not a per-unit identifier.
 *
 * `ble` is the BLE MAC address, only emitted on BLE-capable builds.
 *
 * Both are optional for compatibility with older firmware that predates
 * cross-transport identity.
 */
export const DeviceInfoSchema = z.object({
  version: z.string().min(1, 'Version is required'),
  device: DeviceConfigSchema,
  sn: z.string().optional(),
  ble: z.string().optional(),
});

export type DeviceInfo = z.infer<typeof DeviceInfoSchema>;

/**
 * Helper type for configured device
 */
export type ConfiguredDevice = z.infer<typeof ConfiguredDeviceSchema>;

/**
 * Type guard to check if device is configured
 */
export function isDeviceConfigured(device: DeviceInfo['device']): device is ConfiguredDevice {
  return device.configured === true;
}
