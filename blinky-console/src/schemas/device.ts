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
 * Device information from `json info` command (v28+ format)
 */
export const DeviceInfoSchema = z.object({
  version: z.string().min(1, 'Version is required'),
  device: DeviceConfigSchema,
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
