/**
 * Battery streaming Zod schemas
 *
 * Validates streaming battery messages `{"b":{...}}` from the device.
 */

import { z } from 'zod';

/**
 * Battery status from streaming `{"b":{...}}` messages
 */
export const BatterySampleSchema = z.object({
  n: z.boolean(), // connected (battery detected)
  c: z.boolean(), // charging
  v: z.number().min(0).max(5), // voltage (typically 3.0-4.2V for LiPo)
  p: z.number().int().min(0).max(100), // percent (0-100)
});

export type BatterySample = z.infer<typeof BatterySampleSchema>;

/**
 * Streaming battery message format
 */
export const BatteryMessageSchema = z.object({
  b: BatterySampleSchema,
});

export type BatteryMessage = z.infer<typeof BatteryMessageSchema>;

/**
 * Battery status data (from `battery` command response)
 */
export const BatteryStatusDataSchema = z.object({
  voltage: z.number().min(0).max(5),
  percent: z.number().int().min(0).max(100),
  charging: z.boolean(),
  connected: z.boolean(),
});

export type BatteryStatusData = z.infer<typeof BatteryStatusDataSchema>;

/**
 * Battery status response wrapper
 */
export const BatteryStatusResponseSchema = z.object({
  battery: BatteryStatusDataSchema,
});

export type BatteryStatusResponse = z.infer<typeof BatteryStatusResponseSchema>;
